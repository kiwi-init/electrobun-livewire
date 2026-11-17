// expect: 11 bytes
// expect: héllo 🌍
// expect: replacement ok
const bytes = new TextEncoder().encode("héllo 🌍");
console.log(bytes.length + " bytes");
console.log(new TextDecoder().decode(bytes));
const bad = new TextDecoder().decode(new Uint8Array([0x61, 0xff, 0x62]));
console.log(bad === "a\ufffdb" ? "replacement ok" : JSON.stringify(bad));
