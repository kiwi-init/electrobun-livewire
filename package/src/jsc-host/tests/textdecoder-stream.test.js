// expect: €
// expect: a€b
// expect: bom stripped 1
// expect: bom kept 2
const euro = new TextEncoder().encode("€");
const d = new TextDecoder();
console.log(d.decode(euro.subarray(0, 2), { stream: true }) + d.decode(euro.subarray(2)));
const d2 = new TextDecoder();
console.log(d2.decode(new Uint8Array([0x61, 0xe2]), { stream: true }) + d2.decode(new Uint8Array([0x82]), { stream: true }) + d2.decode(new Uint8Array([0xac, 0x62])));
console.log("bom stripped", new TextDecoder().decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x61])).length);
console.log("bom kept", new TextDecoder("utf-8", { ignoreBOM: true }).decode(new Uint8Array([0xef, 0xbb, 0xbf, 0x61])).length);
