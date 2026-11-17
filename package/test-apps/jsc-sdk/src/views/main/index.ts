import Electrobun, { Electroview } from "electrobun/view";
import type { TestRPC } from "../../shared/rpc";

const rpc = Electroview.defineRPC<TestRPC>({
	maxRequestTime: 5000,
	handlers: {
		requests: { add: ({ a, b }) => a + b },
		messages: {},
	},
});
const view = new Electrobun.Electroview({ rpc });

(async () => {
	const reply = await view.rpc!.request.hello({ name: "the page" });
	document.getElementById("reply")!.textContent = reply;
	view.rpc!.send.ready({});
})();
