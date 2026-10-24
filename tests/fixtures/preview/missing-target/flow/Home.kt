fun Home() {
  Column {
    Text("Hi")
    Button("Go", onClick = { nav("Ghost") })
  }
}
