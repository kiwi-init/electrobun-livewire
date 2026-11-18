// bun:sqlite on the Livewire JSC runtime, over the system libsqlite3
// (sqlite.c): Database, Statement and SQLiteError, with Bun's API: run/exec,
// prepare/query (cached), get/all/values/iterate, named and positional
// parameters, transactions, serialize/deserialize.
const s = globalThis.__sqlite;

export const constants = {
	SQLITE_OPEN_READONLY: 0x1, SQLITE_OPEN_READWRITE: 0x2, SQLITE_OPEN_CREATE: 0x4, SQLITE_OPEN_URI: 0x40,
	SQLITE_OPEN_MEMORY: 0x80, SQLITE_OPEN_NOMUTEX: 0x8000, SQLITE_OPEN_FULLMUTEX: 0x10000,
	SQLITE_PREPARE_PERSISTENT: 0x1,
};

export class SQLiteError extends Error {
	constructor(message, options) {
		super(message);
		this.name = "SQLiteError";
		if (options) Object.assign(this, options);
	}
}
// Natives throw plain Errors named SQLiteError; give them this class.
const wrap = (fn) => {
	try { return fn(); } catch (e) {
		if (e && e.name === "SQLiteError" && !(e instanceof SQLiteError)) {
			const err = new SQLiteError(e.message, { code: e.code, errno: e.errno });
			err.stack = e.stack;
			throw err;
		}
		throw e;
	}
};

// Values in the order of the statement's parameters: positional (spread or
// one array), or one object keyed with the prefix ($a, :a, @a) or, as in
// strict mode, without it.
function bindOrder(stmt, args) {
	if (args.length === 0) return [];
	const toNative = (v) => (typeof v === "bigint" ? { bigint: v.toString() } : v);
	const first = args[0];
	const named = args.length === 1 && first !== null && typeof first === "object" && !Array.isArray(first) && !ArrayBuffer.isView(first) && !(first instanceof ArrayBuffer);
	if (!named) return (args.length === 1 && Array.isArray(first) ? first : args).map(toNative);
	return stmt._params.map((name, i) => {
		if (name === null) {
			if (String(i + 1) in first) return toNative(first[String(i + 1)]);
			return undefined;
		}
		if (name in first) return toNative(first[name]);
		const bare = name.slice(1);
		if (bare in first) return toNative(first[bare]);
		if (stmt._db._strict) throw new SQLiteError(`Missing parameter "${bare}"`, { code: "SQLITE_ERROR", errno: 1 });
		return undefined;
	});
}
const fromNative = (row) => {
	if (row === null || typeof row !== "object") return row;
	for (const k in row) { const v = row[k]; if (v && typeof v === "object" && "bigint" in v && !ArrayBuffer.isView(v)) row[k] = BigInt(v.bigint); }
	return row;
};

// A statement nobody finalized is finalized once it's collected, as Bun's
// are. db._statements maps each live handle to its statement's WeakRef: a
// handle number is reused once freed, so the ref must still match.
// FinalizationRegistry callbacks never run on WebKitGTK's JSC here (its
// deferred work waits on a GLib context the host doesn't run), so
// sweepStatements() also finalizes collected ones on prepare and close.
const unreachable = new FinalizationRegistry(({ live, h, ref }) => {
	if (live.get(h) !== ref) return;
	live.delete(h);
	s.finalize(h);
});
function sweepStatements(db) {
	for (const [h, ref] of db._statements) {
		if (ref.deref()) continue;
		db._statements.delete(h);
		s.finalize(h);
	}
	db._sweepAt = Math.max(64, db._statements.size * 2);
}

export class Statement {
	constructor(db, sql, persistent) {
		if (db._statements.size >= db._sweepAt) sweepStatements(db);
		const p = wrap(() => s.prepare(db._handle(), sql, persistent));
		if (!p.handle) throw new SQLiteError("Query contained no valid SQL statement; likely empty query.", { code: "SQLITE_MISUSE", errno: 21 });
		this._db = db;
		this._h = p.handle;
		const ref = new WeakRef(this);
		db._statements.set(p.handle, ref);
		unreachable.register(this, { live: db._statements, h: p.handle, ref }, this);
		this._params = p.params;
		this.columnNames = p.columns;
		this.paramsCount = p.params.length;
		this.native = this;
		this._sql = sql;
	}
	_step(args, mode) {
		if (!this._h) throw new Error("Statement has been finalized");
		const r = wrap(() => s.step(this._h, bindOrder(this, args), mode, this._db._safeIntegers));
		if (!this._db._safeIntegers) return r;
		if (mode === 1) return fromNative(r);
		if (mode >= 2) return r.map(fromNative);
		return r;
	}
	run(...args) { return this._step(args, 0); }
	get(...args) { return this._step(args, 1); }
	all(...args) { return this._step(args, 2); }
	values(...args) { return this._step(args, 3); }
	*iterate(...args) { yield* this._step(args, 2); }
	[Symbol.iterator]() { return this.iterate(); }
	as(Class) {
		const all = this.all.bind(this), get = this.get.bind(this);
		const make = (row) => (row === null ? null : Object.assign(Object.create(Class.prototype), row));
		this.all = (...a) => all(...a).map(make);
		this.get = (...a) => make(get(...a));
		return this;
	}
	finalize() {
		if (!this._h) return;
		s.finalize(this._h);
		this._db._statements.delete(this._h);
		unreachable.unregister(this);
		this._h = 0;
	}
	toString() { return this._h ? s.expanded(this._h) : this._sql; }
}

export class Database {
	constructor(filename = ":memory:", options = {}) {
		if (typeof options === "number") options = { flags: options };
		if (filename === "" || filename == null) filename = ":memory:";
		let flags = options.flags ?? 0;
		if (!flags) {
			if (options.readonly) flags = constants.SQLITE_OPEN_READONLY;
			else {
				flags = constants.SQLITE_OPEN_READWRITE;
				if (options.create !== false) flags |= constants.SQLITE_OPEN_CREATE;
			}
		}
		this.filename = String(filename);
		this._strict = !!options.strict;
		this._safeIntegers = !!options.safeIntegers;
		this._statements = new Map(); // handle -> WeakRef of its Statement
		this._sweepAt = 64;
		this._cache = new Map();
		this._h = options._handle ?? wrap(() => s.open(this.filename, flags));
	}
	static open(filename, options) { return new Database(filename, options); }
	static deserialize(bytes, options) {
		const readonly = typeof options === "boolean" ? options : !!options?.readonly;
		const h = wrap(() => s.deserialize(bytes, readonly));
		const db = new Database(":memory:", { ...(typeof options === "object" ? options : {}), _handle: h });
		return db;
	}
	_handle() {
		if (!this._h) throw new Error("Cannot use a closed database");
		return this._h;
	}
	get handle() { return this._h; }
	get inTransaction() { return this._h ? s.inTransaction(this._h) : false; }
	prepare(sql, params) {
		const stmt = new Statement(this, sql, false);
		if (params !== undefined) {
			const bound = Array.isArray(params) ? params : [params];
			for (const m of ["run", "get", "all", "values"]) { const f = stmt[m].bind(stmt); stmt[m] = (...a) => f(...(a.length ? a : bound)); }
		}
		return stmt;
	}
	query(sql) {
		let stmt = this._cache.get(sql);
		if (stmt && stmt._h) return stmt;
		stmt = new Statement(this, sql, true);
		this._cache.set(sql, stmt);
		return stmt;
	}
	// Every statement in sql, the parameters bound to each.
	run(sql, ...params) {
		let rest = String(sql);
		let result = { changes: 0, lastInsertRowid: 0 };
		while (rest.trim()) {
			const p = wrap(() => s.prepare(this._handle(), rest, false));
			if (!p.handle) break;
			const stmt = { _h: p.handle, _params: p.params, _db: this };
			try { result = wrap(() => s.step(p.handle, bindOrder(stmt, params), 0, false)); }
			finally { s.finalize(p.handle); }
			rest = p.tail;
		}
		return result;
	}
	exec(sql, ...params) { return this.run(sql, ...params); }
	transaction(fn) {
		const db = this;
		let depth = 0;
		const make = (begin) => function (...args) {
			if (db.inTransaction) {
				const name = "livewire_sp_" + ++depth;
				db.run("SAVEPOINT " + name);
				try { const r = fn.apply(this, args); db.run("RELEASE " + name); return r; }
				catch (e) { db.run("ROLLBACK TO " + name); db.run("RELEASE " + name); throw e; }
				finally { depth--; }
			}
			db.run(begin);
			try { const r = fn.apply(this, args); db.run("COMMIT"); return r; }
			catch (e) { if (db.inTransaction) db.run("ROLLBACK"); throw e; }
		};
		const tx = make("BEGIN");
		tx.deferred = make("BEGIN DEFERRED");
		tx.immediate = make("BEGIN IMMEDIATE");
		tx.exclusive = make("BEGIN EXCLUSIVE");
		return tx;
	}
	serialize(name) {
		if (name && name !== "main") throw new Error("serialize: only the main database on the JSC runtime");
		const bytes = wrap(() => s.serialize(this._handle()));
		return typeof Buffer === "function" ? Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength) : bytes;
	}
	loadExtension() { throw new Error("loadExtension isn't available on the JSC runtime"); }
	fileControl() { throw new Error("fileControl isn't available on the JSC runtime"); }
	close(throwOnError = false) {
		if (!this._h) return;
		sweepStatements(this); // collected ones can't be in use: close(true) too
		if (!throwOnError) for (const [, ref] of [...this._statements]) ref.deref()?.finalize();
		wrap(() => s.close(this._h, throwOnError));
		this._h = 0;
		this._cache.clear();
	}
	[Symbol.dispose]() { this.close(); }
	static setCustomSQLite() { throw new Error("setCustomSQLite isn't available on the JSC runtime: it uses the system libsqlite3"); }
}

export default Database;
