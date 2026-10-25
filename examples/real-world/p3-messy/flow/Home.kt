fun Home() {
  Column {
    Text("Messy home")
    Button("Helper nav")
    Button("Cross file")
    Button("Var ref")
    Button("Nested")
    Button("Dynamic", onClick = { nav("Detail/${noteId}") })
    Button("Go", onClick = { nav("Detail") })
    Button("Share")
    Button("Mystery")
  }
  helperButton.setOnClickListener { openDetail() }
  crossButton.setOnClickListener(::onCrossClick)
  val h = ::onVarClick
  varButton.setOnClickListener(h)
  nestedButton.setOnClickListener { runLater { nav("Detail") } }
  mysteryButton.setOnClickListener { nav("Form") }
}

fun openDetail() {
  nav("Detail")
}

fun onVarClick(v: View) {
  nav("Form")
}
