// bun:sqlite: what Channels' search index does (FTS5 in :memory:, prepare,
// query().get/all, transactions, serialize/deserialize) plus the common API.
import { Database, SQLiteError } from "bun:sqlite";

const db = new Database(":memory:");
for (const sql of [
	"CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)",
	"CREATE VIRTUAL TABLE IF NOT EXISTS hits USING fts5(body, who, room UNINDEXED, at UNINDEXED, tokenize = 'unicode61 remove_diacritics 2')",
]) db.run(sql);
const put = (k, v) => db.run("INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT (k) DO UPDATE SET v = excluded.v", [k, v]);
console.log("run", JSON.stringify(put("stamp", "1")), JSON.stringify(put("stamp", "2")));
console.log("get", JSON.stringify(db.query("SELECT v FROM meta WHERE k = ?").get("stamp")), db.query("SELECT v FROM meta WHERE k = ?").get("none"));

const add = db.prepare("INSERT INTO hits (rowid, body, who, room, at) VALUES (?, ?, ?, ?, ?)");
const drop = db.prepare("DELETE FROM hits WHERE rowid = ?");
const rows = [
	[1, "Café deploy failed on staging", "ana", "r1", 100],
	[2, "the deploy is green now", "bo", "r1", 200],
	[3, "lunch?", "ana", "r2", 300],
	[4, "Deploying the café menu", "cy", "r2", 400],
];
db.transaction(() => { for (const r of rows) { drop.run(r[0]); add.run(...r); } })();
console.log("count", db.query("SELECT COUNT(*) AS n FROM hits").get().n, db.inTransaction);
const search = (match, rooms) => db
	.query(`SELECT rowid AS seq, rank, room, who AS "from", body AS text, at FROM hits WHERE hits MATCH ? AND room IN (${rooms.map(() => "?").join(",")}) ORDER BY rank, rowid DESC LIMIT ?`)
	.all(match, ...rooms, 10)
	.map((h) => `${h.seq}:${h.from}:${h.text}:${typeof h.rank}`);
console.log("fts", JSON.stringify(search("deploy*", ["r1", "r2"])), JSON.stringify(search("cafe", ["r2"])));
db.run("UPDATE hits SET body = ? WHERE rowid = ?", ["lunch at noon", 3]);
console.log("updated", JSON.stringify(search("noon", ["r2"])));

// serialize, then a new database from the bytes.
const bytes = db.serialize();
const copy = Database.deserialize(bytes);
console.log("serialize", bytes instanceof Uint8Array, bytes.length > 0, copy.query("SELECT COUNT(*) AS n FROM hits").get().n, JSON.stringify(copy.query("SELECT v FROM meta WHERE k = ?").get("stamp")));
copy.close();

// Rollback on throw, nested transactions as savepoints.
const tx = db.transaction((fail) => {
	db.run("INSERT INTO meta VALUES ('a', 'x')");
	try { db.transaction(() => { db.run("INSERT INTO meta VALUES ('b', 'y')"); throw new Error("inner"); })(); } catch (e) { console.log("inner rolled back:", e.message); }
	if (fail) throw new Error("outer");
});
try { tx(true); } catch (e) { console.log("outer:", e.message); }
console.log("after rollback", JSON.stringify(db.query("SELECT k FROM meta ORDER BY k").values()));
tx(false);
console.log("after commit", JSON.stringify(db.query("SELECT k FROM meta ORDER BY k").values()));

// Types, named parameters, values(), columnNames, errors.
db.run("CREATE TABLE t (i INTEGER, f REAL, s TEXT, b BLOB, n)");
const ins = db.prepare("INSERT INTO t VALUES ($i, $f, $s, $b, $n)");
console.log("named", JSON.stringify(ins.run({ $i: 42, $f: 1.5, $s: "héllo", $b: new Uint8Array([1, 2, 3]), $n: null })));
db.prepare("INSERT INTO t VALUES (?, ?, ?, ?, ?)").run(2 ** 40, -0.25, "", new Uint8Array(0), true);
const typed = db.query("SELECT * FROM t").all();
console.log("types", typed.map((r) => [r.i, r.f, r.s, r.b instanceof Uint8Array ? "u8:" + Array.from(r.b).join(".") : String(r.b), r.n].join("|")).join(" ; "));
console.log("values", JSON.stringify(db.query("SELECT i, s FROM t").values()), JSON.stringify(db.query("SELECT i AS x, s AS y FROM t").columnNames));
console.log("paramsCount", ins.paramsCount, db.query("SELECT 1").get()["1"]);
for (const bad of ["SELECT * FROM nope", "INSERT INTO meta VALUES (NULL, NULL)", "SELEC 1"]) {
	try { db.query(bad).all(); console.log("no error"); } catch (e) { console.log("error", e instanceof SQLiteError, e.name, e.code, e.message); }
}
try { db.run("INSERT INTO meta VALUES ('a', 'dup')"); } catch (e) { console.log("constraint", e.code, e.errno, e.message); }
let iter = 0;
for (const row of db.query("SELECT k FROM meta ORDER BY k").iterate()) iter += row.k.length;
console.log("iterate", iter);
db.close();
try { db.query("SELECT 1"); } catch (e) { console.log("closed:", e.message); }
