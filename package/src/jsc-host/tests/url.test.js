// expect: https://example.com/a/c?x=1&y=two#h
// expect: example.com 443?  /a/c x=1 two
// expect: views://main/index.html main /index.html
// expect: file:///Users/me/app/b.js
// expect: http://localhost:3000/x?q=a+b
// expect: invalid ok
const u = new URL("../c?x=1&y=two#h", "https://Example.com/a/b/d");
console.log(u.href);
console.log(u.hostname, u.port || "443?", "", u.pathname, u.searchParams.toString().split("&")[0], u.searchParams.get("y"));
const v = new URL("views://main/index.html");
console.log(v.href, v.host, v.pathname);
console.log(new URL("b.js", "file:///Users/me/app/a.js").href);
const w = new URL("http://localhost:3000/x");
w.searchParams.set("q", "a b");
console.log(w.toString());
try { new URL("not a url"); } catch (e) { console.log(e instanceof TypeError ? "invalid ok" : "wrong error"); }
