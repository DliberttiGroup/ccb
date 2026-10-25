fun Detail() {
  Column {
    Text("Detail")
    Button("Go", onClick = { nav("Home") })
    Button("Share")
    Button("Back", onClick = { nav("Home") })
  }
}
