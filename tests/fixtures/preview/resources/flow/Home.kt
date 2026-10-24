fun Home() {
  Column {
    Text("@string/title")
    Text("@string/missing")
    Button("Go", onClick = { nav("Detail") })
  }
}
fun Detail() {
  Column {
    Text("Detail")
  }
}
