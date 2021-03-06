/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Make sure that updating the editor mode sets the right highlighting engine,
 * and script URIs with extra query parameters also get the right engine.
 */

const TAB_URL = EXAMPLE_URL + "browser_dbg_update-editor-mode.html";

let tempScope = {};
Cu.import("resource:///modules/source-editor.jsm", tempScope);
let SourceEditor = tempScope.SourceEditor;

var gPane = null;
var gTab = null;
var gDebuggee = null;
var gDebugger = null;
var gScripts = null;

function test()
{
  debug_tab_pane(TAB_URL, function(aTab, aDebuggee, aPane) {
    gTab = aTab;
    gDebuggee = aDebuggee;
    gPane = aPane;
    gDebugger = gPane.debuggerWindow;

    testScriptsDisplay();
  });
}

function testScriptsDisplay() {
  gPane.activeThread.addOneTimeListener("scriptsadded", function() {
    Services.tm.currentThread.dispatch({ run: function() {
      gScripts = gDebugger.DebuggerView.Scripts._scripts;

      is(gDebugger.StackFrames.activeThread.state, "paused",
        "Should only be getting stack frames while paused.");

      is(gScripts.itemCount, 2, "Found the expected number of scripts.");

      is(gDebugger.editor.getMode(), SourceEditor.MODES.HTML,
         "Found the expected editor mode.");

      ok(gDebugger.editor.getText().search(/debugger/) != -1,
        "The correct script was loaded initially.");

      gDebugger.editor.addEventListener(SourceEditor.EVENTS.TEXT_CHANGED,
                                        function onChange() {
        gDebugger.editor.removeEventListener(SourceEditor.EVENTS.TEXT_CHANGED,
                                             onChange);
        testSwitchPaused();
      });
      gScripts.selectedIndex = 0;
      gDebugger.SourceScripts.onChange({ target: gScripts });
    }}, 0);
  });

  gDebuggee.firstCall();
}

function testSwitchPaused()
{
  ok(gDebugger.editor.getText().search(/debugger/) == -1,
    "The second script is no longer displayed.");

  ok(gDebugger.editor.getText().search(/firstCall/) != -1,
    "The first script is displayed.");

  is(gDebugger.editor.getMode(), SourceEditor.MODES.JAVASCRIPT,
     "Found the expected editor mode.");

  gDebugger.StackFrames.activeThread.resume(function() {
    removeTab(gTab);
    finish();
  });
}
