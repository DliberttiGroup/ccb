fun Home() {
  Column {
    Text("Home text")
    Button("Go", onClick = { nav("Other") })
  }
}
