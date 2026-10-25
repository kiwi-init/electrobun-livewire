// Pending agent evaluations, settled by the `agentEvalResult` internal bridge
// message that the injected page script posts back. Kept free of other SDK
// imports so proc/native.ts can depend on it without an import cycle.

export type AgentEvalOutcome =
	| { ok: true; value: unknown }
	| { ok: false; error: string };

type Pending = {
	resolve: (outcome: AgentEvalOutcome) => void;
	timer: ReturnType<typeof setTimeout>;
};

const pending = new Map<string, Pending>();

export function waitForAgentEval(
	token: string,
	timeoutMs: number,
): Promise<AgentEvalOutcome> {
	return new Promise((resolve) => {
		const timer = setTimeout(() => {
			pending.delete(token);
			resolve({
				ok: false,
				error: `no result within ${timeoutMs}ms (the page may have navigated, or the view has no internal bridge)`,
			});
		}, timeoutMs);
		pending.set(token, { resolve, timer });
	});
}

export function settleAgentEval(payload: unknown): void {
	if (!payload || typeof payload !== "object") return;
	const { token, ok, value, error } = payload as {
		token?: unknown;
		ok?: unknown;
		value?: unknown;
		error?: unknown;
	};
	if (typeof token !== "string") return;
	const entry = pending.get(token);
	if (!entry) return;
	pending.delete(token);
	clearTimeout(entry.timer);
	entry.resolve(
		ok === true
			? { ok: true, value }
			: { ok: false, error: typeof error === "string" ? error : String(error) },
	);
}

export function pendingAgentEvalCount(): number {
	return pending.size;
}
