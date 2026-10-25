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
				error: `no result within ${timeoutMs}ms (the page may have navigated or be stuck)`,
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

// The page reported over the event bridge that it has no internal bridge to
// return a result on: its origin isn't in the view's bridgeOrigins, or it's a
// sandboxed page. Only failures are accepted this way, never values, since
// any page can post to the event bridge.
export function settleAgentEvalNoBridge(detail: string): void {
	let token: unknown;
	let url: unknown;
	try {
		({ token, url } = JSON.parse(detail));
	} catch {
		return;
	}
	settleAgentEval({
		token,
		ok: false,
		error: `the page${typeof url === "string" ? ` (${url})` : ""} isn't on the view's bridgeOrigins, so agent control can't get results from it. Add its origin to bridgeOrigins, or navigate back to one that is.`,
	});
}

export function pendingAgentEvalCount(): number {
	return pending.size;
}
