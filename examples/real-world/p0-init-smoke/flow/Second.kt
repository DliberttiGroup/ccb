fun Second() {
  Column {
    Text("Second screen")
    Button("Back", onClick = { nav("Home") })
  }
}
