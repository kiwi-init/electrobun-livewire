// Bun.secrets on the OS store. It writes to the real keychain, so it runs
// only where LIVEWIRE_KEYCHAIN_TEST is set (CI's macOS job).

async function main() {
	if (!process.env.LIVEWIRE_KEYCHAIN_TEST) {
		console.log("skipped: set LIVEWIRE_KEYCHAIN_TEST=1 to use the keychain");
	} else {
		const service = "dev.livewire.conformance." + process.env.LIVEWIRE_KEYCHAIN_TEST;
		const name = "key";
		const opts = { service, name, allowUnrestrictedAccess: true };
		await Bun.secrets.delete(opts);
		console.log("missing", await Bun.secrets.get(opts));
		console.log("set", await Bun.secrets.set({ ...opts, value: "first value" }));
		console.log("get", await Bun.secrets.get(opts));
		await Bun.secrets.set({ ...opts, value: "second: ünïcode ✓" });
		console.log("overwrite", await Bun.secrets.get(opts));
		console.log("other name", await Bun.secrets.get({ service, name: "other" }));
		console.log("delete", await Bun.secrets.delete(opts));
		console.log("delete again", await Bun.secrets.delete(opts));
		console.log("after delete", await Bun.secrets.get(opts));
		await Bun.secrets.set({ ...opts, value: "x" });
		await Bun.secrets.set({ ...opts, value: "" });
		console.log("empty value deletes", await Bun.secrets.get(opts));
	}
	for (const [label, args] of [["no options", []], ["not an object", ["s"]], ["empty service", [{ service: "", name: "n" }]], ["number name", [{ service: "s", name: 1 }]]]) {
		try { await Bun.secrets.get(...args); console.log(label, "no error"); } catch (e) { console.log(label, e.constructor.name, e.code, JSON.stringify(e.message)); }
	}
	try { await Bun.secrets.set({ service: "s", name: "n", value: 5 }); } catch (e) { console.log("number value", e.code, JSON.stringify(e.message)); }
}
main();
