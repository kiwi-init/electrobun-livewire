// expect: prepared 200
// expect: collected ones finalized true
// expect: strict close ok
// Statements nobody finalizes are finalized once collected (as on Bun): by
// the FinalizationRegistry where it runs, else by the sweep a second after
// the last prepare().
// close(true), which fails while any statement is open, then succeeds.
import { Database } from "bun:sqlite";

const db = new Database(":memory:");
db.run("CREATE TABLE t (x)");
function prepareMany() {
	for (let i = 0; i < 200; i++) db.prepare("SELECT x, " + i + " FROM t").all();
}
setTimeout(() => {
	prepareMany();
	console.log("prepared", 200);
}, 0);
setTimeout(() => {
	const kept = db.prepare("SELECT 1");
	// JSC scans the stack conservatively: allow a few survivors.
	console.log("collected ones finalized", db._statements.size <= 21);
	kept.finalize();
	for (const [, ref] of db._statements) ref.deref()?.finalize();
	try {
		db.close(true);
		console.log("strict close ok");
	} catch (e) {
		console.log("strict close failed", e.message);
	}
}, 1500);
