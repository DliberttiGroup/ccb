fun Settings() {
  Column {
    Text("Settings")
    Button("Back", onClick = { nav("Home") })
  }
}
