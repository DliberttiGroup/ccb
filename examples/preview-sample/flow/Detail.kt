fun Detail() {
  Column {
    Text("Note detail")
    when (state) {
      is Loading -> Text("Loading...")
      is Loaded -> Text("Note body")
      else -> Text("Empty")
    }
    Button("Delete", onClick = { nav("Empty") })
    Button("Back", onClick = { nav("Home", currentId) })
  }
}
