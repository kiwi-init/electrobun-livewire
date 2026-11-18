// expect: delay 1
// expect: delay 0
// expect: args a b
// expect: same deadline 1
// expect: same deadline 2
// expect: same deadline 3
// expect: in-pass timeout waits for the next turn
// expect: immediate before it
// expect: next turn
// Timers already overdue when the loop first turns fire by deadline, then in
// creation order (0 counts as 1 ms), as Node and Bun do: not in creation
// order. The 10 ms block makes them all overdue at once, whatever the load.
setTimeout((a, b) => console.log("args", a, b), 2, "a", "b");
setTimeout(() => console.log("delay 1"), 1);
setTimeout(() => console.log("delay 0"), 0);
setTimeout(() => {
	for (let i = 1; i <= 3; i++) setTimeout(() => console.log("same deadline", i), 5);
	Bun.sleepSync(10);
	setTimeout(() => {
		// A timeout made while timers run waits for the next turn, after the
		// immediates; one made before it ran is already due.
		setTimeout(() => console.log("next turn"), 0);
		setImmediate(() => console.log("immediate before it"));
		console.log("in-pass timeout waits for the next turn");
	}, 20);
}, 5);
Bun.sleepSync(10);
