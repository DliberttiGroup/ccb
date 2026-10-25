fun Home() {
  Column {
    Text("@string/notes_title")
    List("notes")
    Button("Open note", onClick = { nav("Detail", "note-42") })
    Button("New note", onClick = { nav("Form") })
    Button("Edit note")
    Button("Quick open")
    Button("Settings")
  }
  editButton.setOnClickListener { nav("Form", "note-42") }
  quickButton.setOnClickListener { navigate(R.id.detail) }
}

fun onSettingsClick(v: View) {
  nav("Settings")
}
