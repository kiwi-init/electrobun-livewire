// GTK3 implementation of the toolkit runtime (wrapper/toolkit.inc).
namespace toolkit {

void disableSetlocale() {
    gtk_disable_setlocale();
}

void init() {
    gtk_init(nullptr, nullptr);
}

void runMainLoop() {
    gtk_main();
}

void quitMainLoop() {
    gtk_main_quit();
}

} // namespace toolkit
