// What a view does with window.open and target=_blank. The index is the value
// the native wrappers take.
export const NEW_WINDOW_POLICIES = ["deny", "external", "popup"] as const;

export type NewWindowPolicy = (typeof NEW_WINDOW_POLICIES)[number];
