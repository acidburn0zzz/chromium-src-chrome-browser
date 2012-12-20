// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Any tasks that communicates with the portable device may take >100ms to
// complete. Those tasks should be run on an blocking thread instead of the
// UI thread.

#include "chrome/browser/system_monitor/portable_device_watcher_win.h"

#include <dbt.h>
#include <portabledevice.h>

#include "base/file_path.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/string_util.h"
#include "base/threading/sequenced_worker_pool.h"
#include "base/utf_string_conversions.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_comptr.h"
#include "chrome/browser/system_monitor/media_storage_util.h"
#include "chrome/browser/system_monitor/removable_device_constants.h"
#include "content/public/browser/browser_thread.h"

namespace chrome {

namespace {

// Name of the client application that communicates with the MTP device.
const char16 kClientName[] = L"Chromium";

// Name of the sequenced task runner.
const char kMediaTaskRunnerName[] = "media-task-runner";

// Returns true if |data| represents a class of portable devices.
bool IsPortableDeviceStructure(LPARAM data) {
  DEV_BROADCAST_HDR* broadcast_hdr =
      reinterpret_cast<DEV_BROADCAST_HDR*>(data);
  if (!broadcast_hdr ||
      (broadcast_hdr->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE)) {
    return false;
  }

  GUID guidDevInterface = GUID_NULL;
  if (FAILED(CLSIDFromString(kWPDDevInterfaceGUID, &guidDevInterface)))
    return false;
  DEV_BROADCAST_DEVICEINTERFACE* dev_interface =
      reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE*>(data);
  return (IsEqualGUID(dev_interface->dbcc_classguid, guidDevInterface) != 0);
}

// Returns the portable device plug and play device ID string.
string16 GetPnpDeviceId(LPARAM data) {
  DEV_BROADCAST_DEVICEINTERFACE* dev_interface =
      reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE*>(data);
  if (!dev_interface)
    return string16();
  string16 device_id(dev_interface->dbcc_name);
  DCHECK(IsStringASCII(device_id));
  return StringToLowerASCII(device_id);
}

// Gets the friendly name of the device specified by the |pnp_device_id|. On
// success, returns true and fills in |name|.
bool GetFriendlyName(const string16& pnp_device_id,
                     IPortableDeviceManager* device_manager,
                     string16* name) {
  DCHECK(device_manager);
  DCHECK(name);
  DWORD name_len = 0;
  HRESULT hr = device_manager->GetDeviceFriendlyName(pnp_device_id.c_str(),
                                                     NULL, &name_len);
  if (FAILED(hr))
    return false;

  hr = device_manager->GetDeviceFriendlyName(
      pnp_device_id.c_str(), WriteInto(name, name_len), &name_len);
  return (SUCCEEDED(hr) && !name->empty());
}

// Gets the manufacturer name of the device specified by the |pnp_device_id|.
// On success, returns true and fills in |name|.
bool GetManufacturerName(const string16& pnp_device_id,
                         IPortableDeviceManager* device_manager,
                         string16* name) {
  DCHECK(device_manager);
  DCHECK(name);
  DWORD name_len = 0;
  HRESULT hr = device_manager->GetDeviceManufacturer(pnp_device_id.c_str(),
                                                     NULL, &name_len);
  if (FAILED(hr))
    return false;

  hr = device_manager->GetDeviceManufacturer(pnp_device_id.c_str(),
                                             WriteInto(name, name_len),
                                             &name_len);
  return (SUCCEEDED(hr) && !name->empty());
}

// Gets the description of the device specified by the |pnp_device_id|. On
// success, returns true and fills in |description|.
bool GetDeviceDescription(const string16& pnp_device_id,
                          IPortableDeviceManager* device_manager,
                          string16* description) {
  DCHECK(device_manager);
  DCHECK(description);
  DWORD desc_len = 0;
  HRESULT hr = device_manager->GetDeviceDescription(pnp_device_id.c_str(), NULL,
                                                    &desc_len);
  if (FAILED(hr))
    return false;

  hr = device_manager->GetDeviceDescription(pnp_device_id.c_str(),
                                            WriteInto(description, desc_len),
                                            &desc_len);
  return (SUCCEEDED(hr) && !description->empty());
}

// On success, returns true and updates |client_info| with a reference to an
// IPortableDeviceValues interface that holds information about the
// application that communicates with the device.
bool GetClientInformation(
    base::win::ScopedComPtr<IPortableDeviceValues>* client_info) {
  HRESULT hr = client_info->CreateInstance(__uuidof(PortableDeviceValues),
                                           NULL, CLSCTX_INPROC_SERVER);
  if (FAILED(hr)) {
    DPLOG(ERROR) << "Failed to create an instance of IPortableDeviceValues";
    return false;
  }

  // Attempt to set client details.
  (*client_info)->SetStringValue(WPD_CLIENT_NAME, kClientName);
  (*client_info)->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 0);
  (*client_info)->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
  (*client_info)->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
  (*client_info)->SetUnsignedIntegerValue(
      WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION);
  (*client_info)->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS,
                                          GENERIC_READ);
  return true;
}

// Opens the device for communication. |pnp_device_id| specifies the plug and
// play device ID string. On success, returns true and updates |device| with a
// reference to the portable device interface.
bool SetUp(const string16& pnp_device_id,
           base::win::ScopedComPtr<IPortableDevice>* device) {
  base::win::ScopedComPtr<IPortableDeviceValues> client_info;
  if (!GetClientInformation(&client_info))
    return false;

  HRESULT hr = device->CreateInstance(__uuidof(PortableDevice), NULL,
                                      CLSCTX_INPROC_SERVER);
  if (FAILED(hr)) {
    DPLOG(ERROR) << "Failed to create an instance of IPortableDevice";
    return false;
  }

  hr = (*device)->Open(pnp_device_id.c_str(), client_info.get());
  if (SUCCEEDED(hr))
    return true;

  if (hr == E_ACCESSDENIED)
    DPLOG(ERROR) << "Access denied to open the device";
  return false;
}

// Returns the unique id property key of the object specified by the
// |object_id|.
REFPROPERTYKEY GetUniqueIdPropertyKey(const string16& object_id) {
  return (object_id == WPD_DEVICE_OBJECT_ID) ?
      WPD_DEVICE_SERIAL_NUMBER : WPD_OBJECT_PERSISTENT_UNIQUE_ID;
}

// Wrapper function to get content property string value. On failure, returns
// an empty string.
string16 GetStringPropertyValue(IPortableDeviceProperties* properties,
                                const string16& object_id,
                                REFPROPERTYKEY key) {
  DCHECK(properties);
  base::win::ScopedComPtr<IPortableDeviceKeyCollection> properties_to_read;
  HRESULT hr = properties_to_read.CreateInstance(
      __uuidof(PortableDeviceKeyCollection), NULL, CLSCTX_INPROC_SERVER);
  if (FAILED(hr)) {
    DPLOG(ERROR) << "Failed to create IPortableDeviceKeyCollection instance";
    return string16();
  }

  if (FAILED(properties_to_read->Add(key)))
    return string16();

  base::win::ScopedComPtr<IPortableDeviceValues> properties_values;
  if (FAILED(properties->GetValues(object_id.c_str(),
                                   properties_to_read.get(),
                                   properties_values.Receive()))) {
    return string16();
  }

  base::win::ScopedCoMem<char16> buffer;
  if (FAILED(properties_values->GetStringValue(key, &buffer)))
    return string16();
  string16 value = static_cast<const char16*>(buffer);
  TrimWhitespace(value, TRIM_ALL, &value);
  return value;
}

// Returns the unique identifier for the object specified by the |object_id|.
string16 GetObjectUniqueId(IPortableDeviceProperties* properties,
                       const string16& object_id) {
  DCHECK(properties);
  REFPROPERTYKEY key = GetUniqueIdPropertyKey(object_id);
  return GetStringPropertyValue(properties, object_id, key);
}

// Constructs the device storage unique identifier using |device_serial_num| and
// |storage_id|. On success, returns true and fills in |device_storage_id|.
bool ConstructDeviceStorageUniqueId(const string16& device_serial_num,
                                    const string16& storage_id,
                                    std::string* device_storage_id) {
  if (device_serial_num.empty() && storage_id.empty())
    return false;

  DCHECK(device_storage_id);
  *device_storage_id = MediaStorageUtil::MakeDeviceId(
       MediaStorageUtil::MTP_OR_PTP,
       UTF16ToUTF8(storage_id + L':' + device_serial_num));
  return true;
}

// Gets a list of removable storage object identifiers present in |device|.
// On success, returns true and fills in |storage_object_ids|.
bool GetRemovableStorageObjectIds(
    IPortableDevice* device,
    PortableDeviceWatcherWin::StorageObjectIDs* storage_object_ids) {
  DCHECK(device);
  DCHECK(storage_object_ids);
  base::win::ScopedComPtr<IPortableDeviceCapabilities> capabilities;
  HRESULT hr = device->Capabilities(capabilities.Receive());
  if (FAILED(hr)) {
    DPLOG(ERROR) << "Failed to get IPortableDeviceCapabilities interface";
    return false;
  }

  base::win::ScopedComPtr<IPortableDevicePropVariantCollection> storage_ids;
  hr = capabilities->GetFunctionalObjects(WPD_FUNCTIONAL_CATEGORY_STORAGE,
                                          storage_ids.Receive());
  if (FAILED(hr)) {
    DPLOG(ERROR) << "Failed to get IPortableDevicePropVariantCollection";
    return false;
  }

  DWORD num_storage_obj_ids = 0;
  hr = storage_ids->GetCount(&num_storage_obj_ids);
  if (FAILED(hr))
    return false;

  for (DWORD index = 0; index < num_storage_obj_ids; ++index) {
    PROPVARIANT object_id = {0};
    PropVariantInit(&object_id);
    hr = storage_ids->GetAt(index, &object_id);
    if (SUCCEEDED(hr) && (object_id.pwszVal != NULL) &&
        (object_id.vt == VT_LPWSTR)) {
      storage_object_ids->push_back(object_id.pwszVal);
    }
    PropVariantClear(&object_id);
  }
  return true;
}

// Returns true if the portable device is a Mass Storage Class (MSC) device.
// This is used to avoid duplication of the device in both volume mount watcher
// and portable device watcher.
bool IsMassStoragePortableDevice(IPortableDeviceProperties* properties,
                                 const string16& device_name) {
  string16 device_protocol = GetStringPropertyValue(properties,
                                                    WPD_DEVICE_OBJECT_ID,
                                                    WPD_DEVICE_PROTOCOL);
  if (device_protocol.empty())
    return true;

  // Based on testing, the device WPD_DEVICE_PROTOCOL key value can be one of
  // the following:
  // - "MSC:" (Mass Storage Class).
  // - "MTP:" (Media Transfer Protocol).
  // - "PTP:" (Picture Transfer Protocol).
  if (StartsWith(device_protocol, L"MSC:", false) ||
      (!StartsWith(device_protocol, L"MTP:", false) &&
       !StartsWith(device_protocol, L"PTP:", false)))
    return true;

  // If the device is a volume mounted device, |device_name| will be
  // the volume name.
  return ((device_name.length() >= 2) && (device_name[1] == L':') &&
      (((device_name[0] >= L'A') && (device_name[0] <= L'Z')) ||
          ((device_name[0] >= L'a') && (device_name[0] <= L'z'))));
}

// Returns the name of the device specified by |pnp_device_id|.
string16 GetDeviceNameOnBlockingThread(
    IPortableDeviceManager* portable_device_manager,
    const string16& pnp_device_id) {
  DCHECK(content::BrowserThread::GetBlockingPool()->RunsTasksOnCurrentThread());
  DCHECK(portable_device_manager);
  string16 name;
  GetFriendlyName(pnp_device_id, portable_device_manager, &name) ||
      GetDeviceDescription(pnp_device_id, portable_device_manager, &name) ||
      GetManufacturerName(pnp_device_id, portable_device_manager, &name);
  return name;
}

// Access the device and gets the device storage details. On success, returns
// true and populates |storage_objects| with device storage details.
bool GetDeviceStorageObjectsOnBlockingThread(
    IPortableDevice* device,
    IPortableDeviceProperties* properties,
    PortableDeviceWatcherWin::StorageObjects* storage_objects) {
  DCHECK(content::BrowserThread::GetBlockingPool()->RunsTasksOnCurrentThread());
  DCHECK(device);
  DCHECK(storage_objects);
  // Old MTP devices does not have a valid serial number. |device_serial_num|
  // will be empty in those cases.
  string16 device_serial_num(
      GetObjectUniqueId(properties, WPD_DEVICE_OBJECT_ID));

  PortableDeviceWatcherWin::StorageObjectIDs storage_obj_ids;
  if (!GetRemovableStorageObjectIds(device, &storage_obj_ids))
    return false;
  for (PortableDeviceWatcherWin::StorageObjectIDs::const_iterator id_iter =
       storage_obj_ids.begin(); id_iter != storage_obj_ids.end(); ++id_iter) {
    string16 storage_persistent_id(GetObjectUniqueId(properties, *id_iter));
    if (storage_persistent_id.empty())
      continue;

    std::string device_storage_id;
    if (ConstructDeviceStorageUniqueId(device_serial_num, storage_persistent_id,
                                       &device_storage_id)) {
      storage_objects->push_back(PortableDeviceWatcherWin::DeviceStorageObject(
          *id_iter, device_storage_id));
    }
  }
  return true;
}

// Accesses the device and gets the device details (name, storage info, etc).
// On success returns true and fills in |device_details|. On failure, returns
// false. |pnp_device_id| specifies the plug and play device ID string.
bool GetDeviceInfoOnBlockingThread(
    IPortableDeviceManager* portable_device_manager,
    const string16& pnp_device_id,
    PortableDeviceWatcherWin::DeviceDetails* device_details) {
  DCHECK(content::BrowserThread::GetBlockingPool()->RunsTasksOnCurrentThread());
  DCHECK(portable_device_manager);
  DCHECK(device_details);
  DCHECK(!pnp_device_id.empty());
  base::win::ScopedComPtr<IPortableDevice> device;
  if (!SetUp(pnp_device_id, &device))
    return false;

  base::win::ScopedComPtr<IPortableDeviceContent> content;
  HRESULT hr = device->Content(content.Receive());
  if (FAILED(hr)) {
    DPLOG(ERROR) << "Failed to get IPortableDeviceContent interface";
    return false;
  }

  base::win::ScopedComPtr<IPortableDeviceProperties> properties;
  hr = content->Properties(properties.Receive());
  if (FAILED(hr)) {
    DPLOG(ERROR) << "Failed to get IPortableDeviceProperties interface";
    return false;
  }

  device_details->name = GetDeviceNameOnBlockingThread(portable_device_manager,
                                                       pnp_device_id);
  if (IsMassStoragePortableDevice(properties.get(), device_details->name))
    return false;

  device_details->location = pnp_device_id;
  PortableDeviceWatcherWin::StorageObjects storage_objects;
  return GetDeviceStorageObjectsOnBlockingThread(
      device.get(), properties.get(), &device_details->storage_objects);
}

// Wrapper function to get an instance of portable device manager. On success,
// returns true and fills in |portable_device_mgr|. On failure, returns false.
bool GetPortableDeviceManager(
  base::win::ScopedComPtr<IPortableDeviceManager>* portable_device_mgr) {
  DCHECK(content::BrowserThread::GetBlockingPool()->RunsTasksOnCurrentThread());
  HRESULT hr = portable_device_mgr->CreateInstance(
      __uuidof(PortableDeviceManager), NULL, CLSCTX_INPROC_SERVER);
  if (SUCCEEDED(hr))
    return true;

  // Either there is no portable device support (Windows XP with old versions of
  // Media Player) or the thread does not have COM initialized.
  DCHECK_NE(CO_E_NOTINITIALIZED, hr);
  return false;
}

// Enumerates the attached portable devices. On success, returns true and fills
// in |devices| with the attached portable device details. On failure, returns
// false.
bool EnumerateAttachedDevicesOnBlockingThread(
    PortableDeviceWatcherWin::Devices* devices) {
  DCHECK(content::BrowserThread::GetBlockingPool()->RunsTasksOnCurrentThread());
  DCHECK(devices);
  base::win::ScopedComPtr<IPortableDeviceManager> portable_device_mgr;
  if (!GetPortableDeviceManager(&portable_device_mgr))
    return false;

  // Get the total number of devices found on the system.
  DWORD pnp_device_count = 0;
  HRESULT hr = portable_device_mgr->GetDevices(NULL, &pnp_device_count);
  if (FAILED(hr))
    return false;

  scoped_array<char16*> pnp_device_ids(new char16*[pnp_device_count]);
  ZeroMemory(pnp_device_ids.get(), pnp_device_count);
  hr = portable_device_mgr->GetDevices(pnp_device_ids.get(), &pnp_device_count);
  if (FAILED(hr))
    return false;

  for (DWORD index = 0; index < pnp_device_count; ++index) {
    PortableDeviceWatcherWin::DeviceDetails device_details;
    if (GetDeviceInfoOnBlockingThread(
        portable_device_mgr, pnp_device_ids[index], &device_details))
      devices->push_back(device_details);
  }
  return !devices->empty();
}

// Handles the device attach event message on a media task runner.
// |pnp_device_id| specifies the attached plug and play device ID string. On
// success, returns true and populates |device_details| with device information.
// On failure, returns false.
bool HandleDeviceAttachedEventOnBlockingThread(
    const string16& pnp_device_id,
    PortableDeviceWatcherWin::DeviceDetails* device_details) {
  DCHECK(content::BrowserThread::GetBlockingPool()->RunsTasksOnCurrentThread());
  DCHECK(device_details);
  base::win::ScopedComPtr<IPortableDeviceManager> portable_device_mgr;
  if (!GetPortableDeviceManager(&portable_device_mgr))
    return false;
  // Sometimes, portable device manager doesn't have the new device details.
  // Refresh the manager device list to update its details.
  portable_device_mgr->RefreshDeviceList();
  return GetDeviceInfoOnBlockingThread(portable_device_mgr, pnp_device_id,
                                       device_details);
}

// Constructs and returns a storage path from storage unique identifier.
string16 GetStoragePathFromStorageId(const std::string& storage_unique_id) {
  // Construct a dummy device path using the storage name. This is only used
  // for registering the device media file system.
  DCHECK(!storage_unique_id.empty());
  return UTF8ToUTF16("\\\\" + storage_unique_id);
}

// Registers |hwnd| to receive portable device notification details. On success,
// returns the device notifications handle else returns NULL.
HDEVNOTIFY RegisterPortableDeviceNotification(HWND hwnd) {
  GUID dev_interface_guid = GUID_NULL;
  HRESULT hr = CLSIDFromString(kWPDDevInterfaceGUID, &dev_interface_guid);
  if (FAILED(hr))
    return NULL;
  DEV_BROADCAST_DEVICEINTERFACE db = {
      sizeof(DEV_BROADCAST_DEVICEINTERFACE),
      DBT_DEVTYP_DEVICEINTERFACE,
      0,
      dev_interface_guid
  };
  return RegisterDeviceNotification(hwnd, &db, DEVICE_NOTIFY_WINDOW_HANDLE);
}

}  // namespace


// PortableDeviceWatcherWin ---------------------------------------------------

PortableDeviceWatcherWin::DeviceStorageObject::DeviceStorageObject(
    const string16& temporary_id,
    const std::string& persistent_id)
    : object_temporary_id(temporary_id),
      object_persistent_id(persistent_id) {
}

PortableDeviceWatcherWin::PortableDeviceWatcherWin()
    : notifications_(NULL),
      ALLOW_THIS_IN_INITIALIZER_LIST(weak_ptr_factory_(this)) {
}

PortableDeviceWatcherWin::~PortableDeviceWatcherWin() {
  UnregisterDeviceNotification(notifications_);
}

void PortableDeviceWatcherWin::Init(HWND hwnd) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  notifications_ = RegisterPortableDeviceNotification(hwnd);
  base::SequencedWorkerPool* pool = content::BrowserThread::GetBlockingPool();
  media_task_runner_ = pool->GetSequencedTaskRunnerWithShutdownBehavior(
      pool->GetNamedSequenceToken(kMediaTaskRunnerName),
      base::SequencedWorkerPool::CONTINUE_ON_SHUTDOWN);
  EnumerateAttachedDevices();
}

void PortableDeviceWatcherWin::OnWindowMessage(UINT event_type, LPARAM data) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  if (!IsPortableDeviceStructure(data))
    return;

  string16 device_id = GetPnpDeviceId(data);
  if (event_type == DBT_DEVICEARRIVAL)
    HandleDeviceAttachEvent(device_id);
  else if (event_type == DBT_DEVICEREMOVECOMPLETE)
    HandleDeviceDetachEvent(device_id);
}

void PortableDeviceWatcherWin::EnumerateAttachedDevices() {
  DCHECK(media_task_runner_.get());
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  Devices* devices = new Devices;
  base::PostTaskAndReplyWithResult(
      media_task_runner_,
      FROM_HERE,
      base::Bind(&EnumerateAttachedDevicesOnBlockingThread, devices),
      base::Bind(&PortableDeviceWatcherWin::OnDidEnumerateAttachedDevices,
                 weak_ptr_factory_.GetWeakPtr(), base::Owned(devices)));
}

void PortableDeviceWatcherWin::OnDidEnumerateAttachedDevices(
    const Devices* devices, const bool result) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  DCHECK(devices);
  if (!result)
    return;
  for (Devices::const_iterator device_iter = devices->begin();
       device_iter != devices->end(); ++device_iter) {
    OnDidHandleDeviceAttachEvent(&(*device_iter), result);
  }
}

void PortableDeviceWatcherWin::HandleDeviceAttachEvent(
    const string16& pnp_device_id) {
  DCHECK(media_task_runner_.get());
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  DeviceDetails* device_details = new DeviceDetails;
  base::PostTaskAndReplyWithResult(
      media_task_runner_,
      FROM_HERE,
      base::Bind(&HandleDeviceAttachedEventOnBlockingThread, pnp_device_id,
                 device_details),
      base::Bind(&PortableDeviceWatcherWin::OnDidHandleDeviceAttachEvent,
                 weak_ptr_factory_.GetWeakPtr(), base::Owned(device_details)));
}

void PortableDeviceWatcherWin::OnDidHandleDeviceAttachEvent(
    const DeviceDetails* device_details, const bool result) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  DCHECK(device_details);
  if (!result)
    return;

  const StorageObjects& storage_objects = device_details->storage_objects;
  const string16& name = device_details->name;
  const string16& location = device_details->location;
  DCHECK(!ContainsKey(device_map_, location));
  base::SystemMonitor* system_monitor = base::SystemMonitor::Get();
  DCHECK(system_monitor);
  for (StorageObjects::const_iterator storage_iter = storage_objects.begin();
       storage_iter != storage_objects.end(); ++storage_iter) {
    const std::string& storage_id = storage_iter->object_persistent_id;
    DCHECK(!ContainsKey(storage_map_, storage_id));

    // Keep track of storage id and storage name to see how often we receive
    // empty values.
    MediaStorageUtil::RecordDeviceInfoHistogram(false, storage_id, name);
    if (storage_id.empty() || name.empty())
      return;

    // Device can have several data partitions. Therefore, add the
    // partition identifier to the storage name. E.g.: "Nexus 7 (s10001)"
    string16 storage_name(name + L" (" + storage_iter->object_temporary_id +
        L')');
    storage_map_[storage_id] =
        base::SystemMonitor::RemovableStorageInfo(storage_id, storage_name,
                                                  location);
    system_monitor->ProcessRemovableStorageAttached(
        storage_id, storage_name, GetStoragePathFromStorageId(storage_id));
  }
  device_map_[location] = storage_objects;
}

void PortableDeviceWatcherWin::HandleDeviceDetachEvent(
    const string16& pnp_device_id) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  MTPDeviceMap::iterator device_iter = device_map_.find(pnp_device_id);
  if (device_iter == device_map_.end())
    return;

  base::SystemMonitor* system_monitor = base::SystemMonitor::Get();
  DCHECK(system_monitor);

  const StorageObjects& storage_objects = device_iter->second;
  for (StorageObjects::const_iterator storage_object_iter =
       storage_objects.begin(); storage_object_iter != storage_objects.end();
       ++storage_object_iter) {
    std::string storage_id = storage_object_iter->object_persistent_id;
    MTPStorageMap::iterator storage_map_iter = storage_map_.find(storage_id);
    DCHECK(storage_map_iter != storage_map_.end());
    system_monitor->ProcessRemovableStorageDetached(
        storage_map_iter->second.device_id);
    storage_map_.erase(storage_map_iter);
  }
  device_map_.erase(device_iter);
}

}  // namespace chrome
