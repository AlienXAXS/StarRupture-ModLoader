#pragma once

namespace ConsoleCtrl
{
	// Install the Windows console control handler.
	// Must be called after Cmd_Stop::Register() so RequestExit is already resolved.
	void Install();

	// Remove the console control handler.
	void Remove();
}
