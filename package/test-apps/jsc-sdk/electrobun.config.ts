// The jsc-sdk check app built by Hutch on the system JavaScriptCore: Hutch
// builds it for Bun, then the devkit's postBuild hook (src/jsc-host/post-build.ts)
// moves its main process to the JSC host. assemble.sh builds the same app by
// hand from a core release.
export default {
	app: { name: "JSC SDK", identifier: "dev.livewire.jsc-sdk", version: "0.0.1" },
	runtime: { exitOnLastWindowClosed: true },
	build: {
		mainProcess: "bun",
		bun: { entrypoint: "src/bun/index.ts" },
		views: { main: { entrypoint: "src/views/main/index.ts" } },
		copy: { "src/views/main/index.html": "views/main/index.html" },
		mac: { bundleCEF: false, entitlements: { "com.apple.security.cs.allow-jit": true } },
	},
	scripts: { postBuild: ".hutch/devkit/api/jsc-host/post-build.ts" },
};
