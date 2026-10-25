fun Checkout() {
  Column {
    Text("Checkout")
    TextField("Name")
    TextField("Address")
    TextField("Card")
    Button("Place order", onClick = { nav("Home") })
    Button("Back", onClick = { nav("Cart") })
  }
}
