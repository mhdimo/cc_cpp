#!/usr/bin/env python3
"""A small terminal emulator, so that the screens of the port can be seen.

The bytes the port writes are a stream of frames: every frame erases the
region it draws and draws it again. Looking at the bytes tells nothing about
the screen the user sees, so this replays them on a grid the way a terminal
does.

Only the escapes the port writes are understood: the cursor moves, the
erase of the screen, the erase to the end of the screen and the plain text.

The colours are kept as well: every cell remembers the attributes it was
written with, so that two screens can be compared cell by cell — the text
alone cannot tell a dim word from a grey one.
"""

import re

# The escapes the port writes.
CSI = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")


class Attributes:
    """The attributes of the text that is written.

    The attributes are kept as the codes of the escape sequence that set
    them, so that two cells can be compared without the emulator having to
    know the meaning of every code.
    """

    def __init__(self):
        self.reset()

    def reset(self):
        """Forgets every attribute."""
        self.bold = False
        self.dim = False
        self.italic = False
        self.underline = False
        self.inverse = False
        self.foreground = ""
        self.background = ""

    def key(self):
        """The attributes as a string, for the comparison of two cells."""
        parts = []
        if self.bold:
            parts.append("1")
        if self.dim:
            parts.append("2")
        if self.italic:
            parts.append("3")
        if self.underline:
            parts.append("4")
        if self.inverse:
            parts.append("7")
        if self.foreground:
            parts.append("fg:" + self.foreground)
        if self.background:
            parts.append("bg:" + self.background)
        return ";".join(parts)

    def apply(self, args):
        """Applies the codes of an escape sequence."""
        codes = [int(a) if a.isdigit() else 0 for a in args.split(";")] if args else [0]
        index = 0
        while index < len(codes):
            code = codes[index]
            index += 1
            if code == 0:
                self.reset()
            elif code == 1:
                self.bold = True
            elif code == 2:
                self.dim = True
            elif code == 3:
                self.italic = True
            elif code == 4:
                self.underline = True
            elif code == 7:
                self.inverse = True
            elif code == 22:
                self.bold = False
                self.dim = False
            elif code == 23:
                self.italic = False
            elif code == 24:
                self.underline = False
            elif code == 27:
                self.inverse = False
            elif 30 <= code <= 37:
                self.foreground = str(code - 30)
            elif code == 39:
                self.foreground = ""
            elif 40 <= code <= 47:
                self.background = str(code - 40)
            elif code == 49:
                self.background = ""
            elif 90 <= code <= 97:
                self.foreground = str(code - 90 + 8)
            elif 100 <= code <= 107:
                self.background = str(code - 100 + 8)
            elif code in (38, 48):
                # A colour of twenty-four bits or of the palette.
                if index < len(codes) and codes[index] == 2 and index + 2 < len(codes):
                    colour = "{}.{}.{}".format(codes[index + 1], codes[index + 2], codes[index + 3])
                    index += 4
                elif index < len(codes) and codes[index] == 5 and index + 1 < len(codes):
                    colour = str(codes[index + 1])
                    index += 2
                else:
                    continue
                if code == 38:
                    self.foreground = colour
                else:
                    self.background = colour


class Screen:
    """The grid of a terminal."""

    def __init__(self, columns=80, rows=24):
        self.columns = columns
        self.rows = rows
        self.grid = [[" "] * columns for _ in range(rows)]
        self.attributes = [[""] * columns for _ in range(rows)]
        # Whether every row ends with a newline of its own. A row that does
        # not is the continuation of the line above it: the terminal wrapped
        # it, because it was too wide.
        self.hard_break = [False] * rows
        self.row = 0
        self.column = 0
        self.style = Attributes()

    def _scroll(self):
        self.grid.pop(0)
        self.grid.append([" "] * self.columns)
        self.attributes.pop(0)
        self.attributes.append([""] * self.columns)
        self.hard_break.pop(0)
        self.hard_break.append(False)

    def _put(self, text):
        for ch in text:
            if ch == "\n":
                # The row the cursor leaves ends with a newline of its own.
                self.hard_break[self.row] = True
                self.row += 1
                self.column = 0
                if self.row >= self.rows:
                    self._scroll()
                    self.row = self.rows - 1
                continue
            if ch == "\r":
                self.column = 0
                continue
            if self.column >= self.columns:
                self.column = 0
                self.row += 1
                if self.row >= self.rows:
                    self._scroll()
                    self.row = self.rows - 1
            # The row is written again, so it does not end with a newline of
            # its own until one is written.
            self.hard_break[self.row] = False
            self.grid[self.row][self.column] = ch
            self.attributes[self.row][self.column] = self.style.key()
            self.column += 1

    def _erase_to_end(self):
        for column in range(self.column, self.columns):
            self.grid[self.row][column] = " "
            self.attributes[self.row][column] = ""
        for row in range(self.row + 1, self.rows):
            self.grid[row] = [" "] * self.columns
            self.attributes[row] = [""] * self.columns

    def _erase_screen(self):
        self.grid = [[" "] * self.columns for _ in range(self.rows)]
        self.attributes = [[""] * self.columns for _ in range(self.rows)]

    def feed(self, data: str):
        """Replays the bytes the port writes."""
        at = 0
        for match in CSI.finditer(data):
            self._put(data[at:match.start()])
            at = match.end()
            args = match.group(1)
            command = match.group(2)
            if command == "A":
                count = int(args) if args.isdigit() else 1
                self.row = max(0, self.row - count)
            elif command == "B":
                count = int(args) if args.isdigit() else 1
                self.row = min(self.rows - 1, self.row + count)
            elif command == "C":
                count = int(args) if args.isdigit() else 1
                self.column = min(self.columns - 1, self.column + count)
            elif command == "D":
                count = int(args) if args.isdigit() else 1
                self.column = max(0, self.column - count)
            elif command == "G":
                self.column = max(0, min(self.columns - 1, (int(args) if args.isdigit() else 1) - 1))
            elif command == "H":
                parts = [p for p in args.split(";") if p.isdigit()]
                row = int(parts[0]) if parts else 1
                column = int(parts[1]) if len(parts) > 1 else 1
                self.row = max(0, min(self.rows - 1, row - 1))
                self.column = max(0, min(self.columns - 1, column - 1))
            elif command == "J":
                if args in ("", "0"):
                    self._erase_to_end()
                elif args == "2":
                    self._erase_screen()
            elif command == "K":
                for column in range(self.column, self.columns):
                    self.grid[self.row][column] = " "
                    self.attributes[self.row][column] = ""
            elif command == "m":
                self.style.apply(args)
            elif command == "h" or command == "l":
                pass  # The cursor of the terminal and the bracketed paste.
        self._put(data[at:])

    def resize(self, columns, rows):
        """Resizes the screen, wrapping the text that is on it again.

        The terminal wraps the text that is on the screen again when it is
        resized: a line that was wider than the terminal takes a row for
        every part of it that does not fit.

        The cursor keeps the place it has in its line, the way a terminal
        does.
        """
        # The lines of the screen: a row that ends with a newline of its own
        # ends a line, a row that does not is the continuation of the line
        # above it.
        logical = []
        begins = []
        current = ""
        start = 0
        for number, row in enumerate(self.grid):
            current += "".join(row).rstrip()
            if number < len(self.hard_break) and self.hard_break[number]:
                logical.append(current)
                begins.append(start)
                current = ""
                start = number + 1
        if current:
            logical.append(current)
            begins.append(start)
        if not logical:
            # The screen holds nothing yet.
            logical.append("")
            begins.append(0)

        # The line the cursor is on, and the place it has in that line.
        cursor_row = max(0, min(self.row, len(self.grid) - 1))
        line = 0
        for number in range(cursor_row + 1):
            if number < len(self.hard_break) and self.hard_break[number]:
                line += 1
        line = min(line, len(logical) - 1)
        offset = self.column
        for number in range(begins[line], cursor_row):
            offset += len("".join(self.grid[number]).rstrip())

        # The lines are wrapped again at the new width.
        grid = []
        starts = []
        lasts = []
        for text in logical:
            pieces = [text[at:at + columns] for at in range(0, len(text), columns)] or [""]
            starts.append(len(grid))
            for piece in pieces:
                grid.append(list(piece.ljust(columns)))
            lasts.append(len(grid) - 1)

        # The last row of every line ends it with a newline of its own.
        breaks = [False] * len(grid)
        for last in lasts:
            breaks[last] = True

        cursor = starts[line] + offset // columns

        # The rows that do not fit are pushed into the scrollback: the screen
        # keeps the last ones.
        if len(grid) > rows:
            dropped = len(grid) - rows
            grid = grid[dropped:]
            breaks = breaks[dropped:]
            cursor -= dropped

        self.columns = columns
        self.rows = rows
        self.grid = grid + [[" "] * columns for _ in range(rows - len(grid))]
        self.attributes = [[""] * columns for _ in range(rows)]
        self.hard_break = breaks + [False] * (rows - len(breaks))
        self.row = max(0, min(cursor, rows - 1))
        self.column = min(self.column, columns - 1)

    def text(self) -> str:
        """The screen, as the lines of text."""
        return "\n".join("".join(row).rstrip() for row in self.grid)

    def cells(self):
        """The screen, as the cells: the character and the attributes."""
        return [(self.grid[row][column], self.attributes[row][column])
                for row in range(self.rows) for column in range(self.columns)]

    def lines(self):
        """The lines of the screen, without the empty ones at the ends."""
        out = ["".join(row).rstrip() for row in self.grid]
        while out and not out[0]:
            out.pop(0)
        while out and not out[-1]:
            out.pop()
        return out
