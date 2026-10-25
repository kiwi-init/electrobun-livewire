ELECTROBUN_EXPORT const char* openFileDialog(const char* startingFolder, const char* allowedFileTypes, int canChooseFiles, int canChooseDirectories, int allowsMultipleSelection) {
    // This function needs to run on the main thread
    return dispatch_sync_main([&]() -> const char* {
        // Determine the file chooser action based on parameters
        GtkFileChooserAction action;
        const char* buttonLabel;
        
        if (canChooseFiles && canChooseDirectories) {
            action = GTK_FILE_CHOOSER_ACTION_OPEN;
            buttonLabel = "_Open";
        } else if (canChooseDirectories) {
            action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
            buttonLabel = "_Select";
        } else {
            action = GTK_FILE_CHOOSER_ACTION_OPEN;
            buttonLabel = "_Open";
        }
        
        LinuxNativeFileDialog dialog(
            "Open File",
            nullptr, // No parent window for now
            action,
            buttonLabel
        );
        if (!dialog.valid()) {
            static std::string emptyResult = "[]";
            return emptyResult.c_str();
        }
        
        // Set starting folder if provided
        if (startingFolder && strlen(startingFolder) > 0) {
            dialog.setCurrentFolder(startingFolder);
        }
        
        // Allow multiple selection if requested
        dialog.setSelectMultiple(allowsMultipleSelection != 0);
        
        // Set up file filters if provided
        if (allowedFileTypes && strlen(allowedFileTypes) > 0) {
            // Parse the allowed file types string (expected format: "*.jpg,*.png" or "Images|*.jpg;*.png|Documents|*.pdf;*.doc")
            std::string typesStr(allowedFileTypes);
            
            // Simple parsing - just handle comma-separated extensions for now
            GtkFileFilter* filter = gtk_file_filter_new();
            gtk_file_filter_set_name(filter, "Allowed files");
            
            // Split by comma or semicolon
            size_t pos = 0;
            std::string delimiter = ",";
            while ((pos = typesStr.find(delimiter)) != std::string::npos) {
                std::string pattern = typesStr.substr(0, pos);
                // Trim whitespace
                pattern.erase(0, pattern.find_first_not_of(" \t"));
                pattern.erase(pattern.find_last_not_of(" \t") + 1);
                
                gtk_file_filter_add_pattern(filter, pattern.c_str());
                typesStr.erase(0, pos + delimiter.length());
            }
            // Add the last pattern
            if (!typesStr.empty()) {
                typesStr.erase(0, typesStr.find_first_not_of(" \t"));
                typesStr.erase(typesStr.find_last_not_of(" \t") + 1);
                gtk_file_filter_add_pattern(filter, typesStr.c_str());
            }
            
            dialog.addFilter(filter);
            
            // Also add "All files" filter
            GtkFileFilter* allFilter = gtk_file_filter_new();
            gtk_file_filter_set_name(allFilter, "All files");
            gtk_file_filter_add_pattern(allFilter, "*");
            dialog.addFilter(allFilter);
        }
        
        // Run the dialog
        std::vector<std::string> paths;

        if (dialog.run() == GTK_RESPONSE_ACCEPT) {
            paths = dialog.selectedPaths();
        }

        static std::string resultString; // Static to persist after function returns.
        resultString = serializeDialogPaths(paths);
        return resultString.c_str();
    });
}

ELECTROBUN_EXPORT int showMessageBox(const char *type,
                   const char *title,
                   const char *message,
                   const char *detail,
                   const char *buttons,
                   int defaultId,
                   int cancelId) {
    return dispatch_sync_main([&]() -> int {
        // Determine message type for GTK
        GtkMessageType messageType = GTK_MESSAGE_INFO;
        if (type) {
            std::string typeStr(type);
            if (typeStr == "warning") {
                messageType = GTK_MESSAGE_WARNING;
            } else if (typeStr == "error" || typeStr == "critical") {
                messageType = GTK_MESSAGE_ERROR;
            } else if (typeStr == "question") {
                messageType = GTK_MESSAGE_QUESTION;
            }
        }

        // Create dialog with no default buttons - we'll add custom ones
        GtkWidget* dialog = gtk_message_dialog_new(
            nullptr, // No parent window
            GTK_DIALOG_MODAL,
            messageType,
            GTK_BUTTONS_NONE,
            "%s",
            message ? message : ""
        );

        // Set title
        if (title && strlen(title) > 0) {
            gtk_window_set_title(GTK_WINDOW(dialog), title);
        }

        // Add secondary text (detail)
        if (detail && strlen(detail) > 0) {
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", detail);
        }

        // Parse and add custom buttons
        std::vector<std::string> buttonLabels;
        if (buttons && strlen(buttons) > 0) {
            std::string buttonsStr(buttons);
            std::stringstream ss(buttonsStr);
            std::string buttonLabel;
            while (std::getline(ss, buttonLabel, ',')) {
                // Trim whitespace
                size_t start = buttonLabel.find_first_not_of(" \t");
                size_t end = buttonLabel.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    buttonLabels.push_back(buttonLabel.substr(start, end - start + 1));
                }
            }
        }
        if (buttonLabels.empty()) {
            buttonLabels.push_back("OK");
        }

        // Add buttons in order (response IDs start at 0)
        for (size_t i = 0; i < buttonLabels.size(); i++) {
            gtk_dialog_add_button(GTK_DIALOG(dialog), buttonLabels[i].c_str(), static_cast<int>(i));
        }

        // Set default button
        if (defaultId >= 0 && defaultId < static_cast<int>(buttonLabels.size())) {
            gtk_dialog_set_default_response(GTK_DIALOG(dialog), defaultId);
        }

        // Run dialog and get response
        int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);

        // Handle GTK response codes
        if (response == GTK_RESPONSE_DELETE_EVENT) {
            // User closed the dialog via window manager
            return cancelId >= 0 ? cancelId : -1;
        }
        if (response >= 0 && response < static_cast<int>(buttonLabels.size())) {
            return response;
        }

        return -1;
    });
}

