// Background mode: the app never takes the user's keyboard focus or screen.
// Windows still open and work, but showing or activating one orders it front
// without activating the app or making it key, and full screen (which moves
// the window to its own Space) is refused. Turned on with
// ELECTROBUN_BACKGROUND=1; Kitchen does this for local runs, so tests can run
// while someone keeps working on the same machine.

let warnedFullScreen = false;

export function isBackgroundMode(): boolean {
	return process.env["ELECTROBUN_BACKGROUND"] === "1";
}

// Whether a window may enter full screen; logs once when background mode
// refuses it.
export function allowsFullScreen(fullScreen: boolean): boolean {
	if (!fullScreen || !isBackgroundMode()) return true;
	if (!warnedFullScreen) {
		warnedFullScreen = true;
		console.warn("[electrobun] full screen is off in background mode (ELECTROBUN_BACKGROUND=1)");
	}
	return false;
}
