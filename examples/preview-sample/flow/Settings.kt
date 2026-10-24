fun Settings() {
  Column {
    Text("Settings")
    TextField("Username")
    if (darkMode) {
      Text("Dark mode on")
    } else {
      Text("Dark mode off")
    }
    Button("Back", onClick = { nav("Home") })
  }
}
