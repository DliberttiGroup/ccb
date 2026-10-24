fun Home() {
  Column {
    Text("My Notes")
    List("notes")
    Row {
      Button("Open note", onClick = { nav("Detail", "note-42") })
      Button("New note", onClick = { nav("Form") })
    }
    Button("Settings", onClick = { nav("Settings") })
  }
}
