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

} // namespace toolkit
