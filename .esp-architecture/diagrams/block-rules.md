# Block diagram rules (working file)

Rules agreed so far. The draft: [block-draft.md](block-draft.md). Temporary, like its branch.

1. **Subject.** The diagram shows the pieces from the hardware to the product function. It has no `main`, and it does not show who controls whom.
2. **Outside boxes.** One per device the processor talks to directly, over wires or over its own radio. A device reached through another device is not drawn: the server and the NTRIP caster are behind the modem.
3. **Communication boxes.** One per communication resource. A bus shared by several devices may be drawn once per device, so its lines stay short.
4. **Bus owner.** Each shared bus has exactly one driver box, and all of the bus's communication boxes connect to it. No other box touches the bus.
5. **Lines.** A line means that two boxes belong next to each other, because one uses the other directly. It replaces position, has no arrow and no label, and does not tell data from control.
