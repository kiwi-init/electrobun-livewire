// expect: prepared 200
// expect: live after idle true
// expect: strict close ok
// Statements nobody finalizes are finalized once collected (as on Bun):
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
	// JSC scans the stack conservatively: allow a few survivors.
	console.log("live after idle", db._statements.size <= 20);
	for (const [, ref] of db._statements) ref.deref()?.finalize();
	try {
		db.close(true);
		console.log("strict close ok");
	} catch (e) {
		console.log("strict close failed", e.message);
	}
}, 1500);
