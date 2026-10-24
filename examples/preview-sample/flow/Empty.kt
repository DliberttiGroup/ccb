fun Empty() {
  Column {
    Text("No notes yet")
    Button("Create one", onClick = { nav("Form") })
  }
}
