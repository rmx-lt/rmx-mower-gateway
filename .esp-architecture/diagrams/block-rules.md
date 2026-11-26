# Block diagram rules (working file)

Rules agreed so far. The draft: [block-draft.md](block-draft.md). Temporary, like its branch.

1. **Subject.** The diagram shows the pieces from the hardware to the product function. It has no `main`, and it does not show who controls whom.
2. **Outside boxes.** One per device the processor talks to directly, over wires or over its own radio. A device reached through another device is not drawn: the server and the NTRIP caster are behind the modem.
3. **Communication boxes.** One per communication resource. A bus shared by several devices may be drawn once per device, so its lines stay short.
4. **Bus owner.** Each shared bus has exactly one driver box, and all of the bus's communication boxes connect to it. No other box touches the bus.
5. **Lines.** A line means that two boxes belong next to each other, because one uses the other directly. It replaces position, has no arrow and no label, and does not tell data from control.
6. **Aspects.** An aspect is one thing about an outside device that the firmware stores, exchanges, switches or senses. Every connection has at least one aspect of its own, and never shares it. A connection is one bus (however many wires) or one GPIO pin. So four GPIO pins are four aspects, and one SPI bus may have several.
   - Stored: one aspect per region with its own purpose.
   - Exchanged: one aspect per bus, whatever the messages contain. Two directions are one aspect.
   - Switched or sensed: one aspect per pin.
7. **Aspect groups.** The aspects that use the same communication box sit in one inner box. With more than one aspect, the inner box gets a title and a separator line. GPIO aspects are listed in the order of their pins.
8. **Aspect names.** A lowercase noun of one or two words, and not the bus, pin, protocol or device name. There are no lists inside an aspect.
9. **Drivers.** Each device has exactly one driver, the box next to its communication boxes. One driver may serve several devices when they serve one purpose: the modem driver also switches the LED that shows the modem's data link. It translates between what travels over the connection and what the rest of the firmware asks for. It is named `<device> driver`, or `<aspect> driver` when the device only passes bytes on (USB bridge: console driver). A driver of several devices is named after the main one.
10. **Own code.** Inside the processor, a box other than a communication box is code of this project. Ready-made code is drawn only when it is a device's driver and no driver of this project wraps it. The framework's log and flashing are not drawn.
11. **Colours.** Communication boxes have a solid blue outline, drivers a solid green one. A driver that the system provides, so it is not code of this project, has a dashed green outline. Other boxes have no colour.
12. **A block inside a block** is owned by it: only the outer block uses it, and lines from outside go to the outer block. The outer block is a container with a grey fill and a solid outline. An outer block that is a driver keeps its green outline. A container without a fill only groups.
13. **Layout.** Blocks inside a block sit in a grid of two rows; an odd count leaves one cell empty.
