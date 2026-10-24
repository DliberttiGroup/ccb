fun Home() {
  Column { Button("A") Button("B") Button("C") Button("D") }
  aButton.setOnClickListener { nav("Detail", "note-42") }
  bButton.setOnClickListener { nav("Detail", id = 7) }
  cButton.setOnClickListener { nav("Detail", noteId) }
  dButton.setOnClickListener { nav(target, "note-42") }
}
fun Detail() {
  Column { Text("Detail") }
}
