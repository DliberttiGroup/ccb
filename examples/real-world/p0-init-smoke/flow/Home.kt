fun Home() {
  Column {
    Text("Hello from CCB!")
    Button("Open second", onClick = { nav("Second") })
  }
}
