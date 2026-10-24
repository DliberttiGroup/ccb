fun Form() {
  Column {
    Text("New note")
    TextField("Title")
    TextField("Body")
    Button("Save", onClick = { nav("Home") })
    Button("More", onClick = { nav(nextScreen) })
  }
}
