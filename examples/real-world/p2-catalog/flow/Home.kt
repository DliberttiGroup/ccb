fun Home() {
  Column {
    Text("Shop")
    TextField("Search")
    List("products")
    Button("Product 1", onClick = { nav("Detail", "p1") })
    Button("Product 2", onClick = { nav("Detail", "p2") })
    Button("Product 3", onClick = { nav("Detail", "p3") })
    Button("Product 4", onClick = { nav("Detail", "p4") })
    Button("Product 5", onClick = { nav("Detail", "p5") })
    Button("Product 6", onClick = { nav("Detail", "p6") })
    Button("Cart", onClick = { nav("Cart") })
  }
}
