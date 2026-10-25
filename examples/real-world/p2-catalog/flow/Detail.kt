fun Detail() {
  Column {
    Text("Product")
    Image("Product image")
    Text("$19.99")
    when (state) {
      is Loading -> Text("Loading...")
      is Loaded -> Text("In stock")
      else -> Text("Unknown")
    }
    Button("Add to cart", onClick = { nav("Cart") })
    Button("Back", onClick = { nav("Home") })
  }
}
