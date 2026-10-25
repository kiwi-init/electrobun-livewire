import ElectrobunEvent from "./event";

type MenuClickedData = { id?: number; action: string; data?: unknown };
type OpenUrlData = { url: string };

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
	beforeQuit: (data: {}) =>
		new ElectrobunEvent<{}, { allow: boolean }>("before-quit", data),
};
