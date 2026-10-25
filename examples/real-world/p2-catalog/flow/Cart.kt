fun Cart() {
  Column {
    Text("Cart")
    List("items")
    Button("Checkout", onClick = { nav("Checkout") })
    Button("Continue shopping", onClick = { nav("Home") })
  }
}
