import ElectrobunEvent from "./event";

type TrayClickedData = { id: number; action: string; data?: unknown };
type TrayHostChangedData = { available: boolean };

export default {
	trayClicked: (data: TrayClickedData) =>
		new ElectrobunEvent<TrayClickedData, { allow: boolean }>(
			"tray-clicked",
			data,
		),
	// Linux: a tray host (Waybar, KDE, the GNOME AppIndicator extension)
	// appeared or went away. See Tray.hostAvailable().
	trayHostChanged: (data: TrayHostChangedData) =>
		new ElectrobunEvent<TrayHostChangedData, void>("tray-host-changed", data),
};
