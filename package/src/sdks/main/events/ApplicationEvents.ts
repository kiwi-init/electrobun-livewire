import ElectrobunEvent from "./event";

type MenuClickedData = { id?: number; action: string; data?: unknown };
type OpenUrlData = { url: string };
// `action` is "default" (the notification itself was clicked), "dismiss", or
// the id of the action button or reply field; `reply` is the text typed.
type NotificationResponseData = { id: string; action: string; reply?: string };

export default {
	applicationMenuClicked: (data: MenuClickedData) =>
		new ElectrobunEvent<MenuClickedData, { allow: boolean }>(
			"application-menu-clicked",
			data,
		),
	contextMenuClicked: (data: MenuClickedData) =>
		new ElectrobunEvent<MenuClickedData, { allow: boolean }>(
			"context-menu-clicked",
			data,
		),
	openUrl: (data: OpenUrlData) =>
		new ElectrobunEvent<OpenUrlData, void>("open-url", data),
	reopen: (data: {}) => new ElectrobunEvent<{}, void>("reopen", data),
	// The Mac woke from sleep. Set `event.response = { recover: false }` to
	// skip probing webviews and recycling frozen ones.
	wake: (data: {}) => new ElectrobunEvent<{}, { recover?: boolean }>("did-wake", data),
	notificationResponse: (data: NotificationResponseData) =>
		new ElectrobunEvent<NotificationResponseData, void>("notification-response", data),
	beforeQuit: (data: {}) =>
		new ElectrobunEvent<{}, { allow: boolean }>("before-quit", data),
};
