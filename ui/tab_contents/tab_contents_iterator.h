// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_TAB_CONTENTS_TAB_CONTENTS_ITERATOR_H_
#define CHROME_BROWSER_UI_TAB_CONTENTS_TAB_CONTENTS_ITERATOR_H_

#include <set>

#include "base/basictypes.h"
#include "chrome/browser/ui/browser_list.h"

namespace content {
class WebContents;
}

// Iterates through all web view hosts in all browser windows. Because the
// renderers act asynchronously, getting a host through this interface does
// not guarantee that the renderer is ready to go. Doing anything to affect
// browser windows or tabs while iterating may cause incorrect behavior.
//
// Example:
//   for (TabContentsIterator iterator; !iterator.done(); ++iterator) {
//     WebContents* cur = *iterator;
//     -or-
//     iterator->OperationOnWebContents();
//     ...
//   }
class TabContentsIterator {
 public:
  TabContentsIterator();

  // Returns true if we are past the last Browser.
  bool done() const { return cur_ == NULL; }

  // Returns the Browser instance associated with the current
  // WebContents. Valid as long as !done()
  Browser* browser() const {
    if (browser_iterator_ != BrowserList::end())
      return *browser_iterator_;
    return NULL;
  }

  // Returns the current WebContents, valid as long as !Done()
  content::WebContents* operator->() const {
    return cur_;
  }
  content::WebContents* operator*() const {
    return cur_;
  }

  // Incrementing operators, valid as long as !Done()
  content::WebContents* operator++() {  // ++preincrement
    Advance();
    return cur_;
  }
  content::WebContents* operator++(int) {  // postincrement++
    content::WebContents* tmp = cur_;
    Advance();
    return tmp;
  }

 private:
  // Loads the next host into |cur_|. This is designed so that for the initial
  // call when browser_iterator_ points to the first browser and
  // web_view_index_ is -1, it will fill the first host.
  void Advance();

  // Iterator over all the Browser objects.
  BrowserList::const_iterator browser_iterator_;

  // Tab index into the current Browser of the current web view.
  int web_view_index_;

  // Current WebContents, or NULL if we're at the end of the list. This
  // can be extracted given the browser iterator and index, but it's nice to
  // cache this since the caller may access the current host many times.
  content::WebContents* cur_;

  DISALLOW_COPY_AND_ASSIGN(TabContentsIterator);
};

#endif  // CHROME_BROWSER_UI_TAB_CONTENTS_TAB_CONTENTS_ITERATOR_H_
