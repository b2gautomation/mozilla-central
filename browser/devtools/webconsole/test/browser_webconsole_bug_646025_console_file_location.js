/* vim:set ts=2 sw=2 sts=2 et: */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is DevTools test code.
 *
 * The Initial Developer of the Original Code is Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  David Dahl <ddahl@mozilla.com>
 *  Patrick Walton <pcwalton@mozilla.com>
 *  Julian Viereck <jviereck@mozilla.com>
 *  Mihai Sucan <mihai.sucan@gmail.com>
 *  Panos Astithas <past@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

// Tests that console logging methods display the method location along with
// the output in the console.

const TEST_URI = "http://example.com/browser/browser/devtools/" +
                 "webconsole/test/" +
                 "test-bug-646025-console-file-location.html";

function test() {
  addTab("data:text/html,Web Console file location display test");
  browser.addEventListener("load", onLoad, true);
}

function onLoad(aEvent) {
  browser.removeEventListener(aEvent.type, arguments.callee, true);
  openConsole();
  hudId = HUDService.getHudIdByWindow(content);

  browser.addEventListener("load", testConsoleFileLocation, true);
  content.location = TEST_URI;
}

function testConsoleFileLocation(aEvent) {
  browser.removeEventListener(aEvent.type, arguments.callee, true);

  outputNode = HUDService.hudReferences[hudId].outputNode;

  executeSoon(function() {
    findLogEntry("test-file-location.js");
    findLogEntry("message for level");
    findLogEntry("test-file-location.js:5");
    findLogEntry("test-file-location.js:6");
    findLogEntry("test-file-location.js:7");
    findLogEntry("test-file-location.js:8");
    findLogEntry("test-file-location.js:9");

    finishTest();
  });
}

