fun Home() {
  Column {
    Button("Cond")
    Button("When")
    Button("Chain")
    Button("Nested")
    Button("Act")
    Button("Named")
  }
  condButton.setOnClickListener { if (ready) { nav("Detail") } else { nav("Form") } }
  whenButton.setOnClickListener { when (state) { is Loaded -> { nav("Detail") } else -> nav("Form") } }
  chainButton.setOnClickListener { if (a) nav("Detail") else if (b) nav("Form") else nav("Detail") }
  nestedButton.setOnClickListener { if (ready) { launch { nav("Detail") } } }
  actButton.setOnClickListener { if (ready) startActivity(Intent(this, DetailActivity::class.java)) }
  namedButton.setOnClickListener(::onNamed)
}
fun onNamed(v: View) { if (ready) { nav("Form") } }
fun Detail() {
  Column { Text("Detail") }
}
fun Form() {
  Column { Text("Form") }
}
fun DetailActivity() {
  Column { Text("Detail activity") }
}
