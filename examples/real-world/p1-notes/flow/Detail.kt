fun Detail() {
  Column {
    Text("Note detail")
    when (state) {
      is Loading -> Text("Loading...")
      is Loaded -> Text("Note body")
      else -> Text("Empty")
    }
    Button("Edit", onClick = { nav("Form", "note-42") })
    Button("Open related", onClick = { nav("Detail", relatedId) })
    Button("Back", onClick = { nav("Home") })
  }
}
