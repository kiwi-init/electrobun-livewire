// The toolkit layer. Shared parts (wrapper/*.inc) reach GTK only through the
// functions declared here; a toolkit's parts (wrapper/gtk3/*.inc today, a
// GTK4 set later) define them. Grows as more of the wrapper moves behind it.
namespace toolkit {

// Keeps the toolkit from setting the C locale when it initializes. CEF needs
// this before it starts, since it initializes GTK itself.
void disableSetlocale();

// Initializes the toolkit, once, on the event loop thread.
void init();

// Runs the toolkit's main loop until quitMainLoop().
void runMainLoop();
void quitMainLoop();

// Window state for toolkit windows (the window exports handle the X11
// windows CEF uses themselves). Called on the main thread with a toolkit
// window; getters write `result` or the out-parameters only when they know.
void closeWindow(void* window);
void requestWindowClose(void* window);
void minimizeWindow(void* window);
void restoreWindow(void* window);
void isWindowMinimized(void* window, bool& result);
void maximizeWindow(void* window);
void unmaximizeWindow(void* window);
void isWindowMaximized(void* window, bool& result);
void setWindowFullScreen(void* window, bool fullScreen);
void isWindowFullScreen(void* window, bool& result);
void setWindowAlwaysOnTop(void* window, bool alwaysOnTop);
void isWindowAlwaysOnTop(void* window, bool& result);
void setWindowPosition(void* window, double x, double y);
void setWindowSize(void* window, double width, double height);
void setWindowFrame(void* window, double x, double y, double width, double height);
void getWindowFrame(void* window, double* outX, double* outY, double* outWidth, double* outHeight);
void getWindowContentOrigin(void* window, double* outX, double* outY);
// Moves a toolkit window to the middle of a monitor's work area.
void centerWindow(void* window, const GdkRectangle& workarea);
void setWindowIcon(void* window, GdkPixbuf* pixbuf);

} // namespace toolkit
