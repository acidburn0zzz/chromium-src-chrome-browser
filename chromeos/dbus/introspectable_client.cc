// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/dbus/introspectable_client.h"

#include <string>
#include <vector>

#include "base/bind.h"
#include "base/logging.h"
#include "chrome/browser/chromeos/system/runtime_environment.h"
#include "chrome/common/libxml_utils.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "dbus/object_path.h"
#include "dbus/object_proxy.h"

namespace {

// D-Bus specification constants.
const char kIntrospectableInterface[] = "org.freedesktop.DBus.Introspectable";
const char kIntrospect[] = "Introspect";

// String constants used for parsing D-Bus Introspection XML data.
const char kInterfaceNode[] = "interface";
const char kInterfaceNameAttribute[] = "name";

}  // namespace

namespace chromeos {

// The IntrospectableClient implementation used in production.
class IntrospectableClientImpl : public IntrospectableClient {
 public:
  explicit IntrospectableClientImpl(dbus::Bus* bus)
      : weak_ptr_factory_(this),
        bus_(bus) {
    DVLOG(1) << "Creating IntrospectableClientImpl";
  }

  virtual ~IntrospectableClientImpl() {
  }

  // IntrospectableClient override.
  virtual void Introspect(const std::string& service_name,
                          const dbus::ObjectPath& object_path,
                          const IntrospectCallback& callback) OVERRIDE {
    dbus::MethodCall method_call(kIntrospectableInterface, kIntrospect);

    dbus::ObjectProxy* object_proxy = bus_->GetObjectProxy(service_name,
                                                           object_path);

    object_proxy->CallMethod(
        &method_call,
        dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::Bind(&IntrospectableClientImpl::OnIntrospect,
                   weak_ptr_factory_.GetWeakPtr(),
                   service_name, object_path, callback));
  }

 private:
  // Called by dbus:: when a response for Introspect() is recieved.
  void OnIntrospect(const std::string& service_name,
                    const dbus::ObjectPath& object_path,
                    const IntrospectCallback& callback,
                    dbus::Response* response) {
    // Parse response.
    bool success = false;
    std::string xml_data;
    if (response != NULL) {
      dbus::MessageReader reader(response);
      if (!reader.PopString(&xml_data)) {
        LOG(WARNING) << "Introspect response has incorrect paramters: "
                     << response->ToString();
      } else {
        success = true;
      }
    }

    // Notify client.
    callback.Run(service_name, object_path, xml_data, success);
  }

  // Weak pointer factory for generating 'this' pointers that might live longer
  // than we do.
  base::WeakPtrFactory<IntrospectableClientImpl> weak_ptr_factory_;

  dbus::Bus* bus_;

  DISALLOW_COPY_AND_ASSIGN(IntrospectableClientImpl);
};

// The IntrospectableClient implementation used on Linux desktop, which does
// nothing.
class IntrospectableClientStubImpl : public IntrospectableClient {
 public:
  // IntrospectableClient override.
  virtual void Introspect(const std::string& service_name,
                          const dbus::ObjectPath& object_path,
                          const IntrospectCallback& callback) OVERRIDE {
    VLOG(1) << "Introspect: " << service_name << " " << object_path.value();
    callback.Run(service_name, object_path, "", false);
  }
};

IntrospectableClient::IntrospectableClient() {
}

IntrospectableClient::~IntrospectableClient() {
}

// static
std::vector<std::string> IntrospectableClient::GetInterfacesFromXmlData(
    const std::string&xml_data) {
  std::vector<std::string> interfaces;

  XmlReader reader;
  if (!reader.Load(xml_data))
    return interfaces;

  do {
    // Skip to the next open tag, exit when done.
    while (!reader.SkipToElement()) {
      if (!reader.Read()) {
        return interfaces;
      }
    }

    // Only look at interface nodes.
    if (reader.NodeName() != kInterfaceNode)
      continue;

    // Skip if missing the interface name.
    std::string interface_name;
    if (!reader.NodeAttribute(kInterfaceNameAttribute, &interface_name))
      continue;

    interfaces.push_back(interface_name);
  } while (reader.Read());

  return interfaces;
}

// static
IntrospectableClient* IntrospectableClient::Create(dbus::Bus* bus) {
  if (system::runtime_environment::IsRunningOnChromeOS()) {
    return new IntrospectableClientImpl(bus);
  } else {
    return new IntrospectableClientStubImpl();
  }
}

}  // namespace chromeos
