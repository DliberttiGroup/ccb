fun Home() {
  Column {
    Button("Open note")
    Button("Detail button")
    Button("Named button")
    Button("Activity button")
  }
  openButton.setOnClickListener { nav("Detail") }
  detailButton.onClick = { nav("Form") }
  namedButton.setOnClickListener(::onOpenClick)
  activityButton.setOnClickListener { startActivity(Intent(this, DetailActivity::class.java)) }
  strayButton.setOnClickListener { nav("Form") }
}
fun onOpenClick(v: View) { nav("Form") }
fun Detail() {
  Column {
    Text("Detail")
  }
}
fun Form() {
  Column {
    Text("Form")
  }
}
fun DetailActivity() {
  Column {
    Text("Detail activity")
  }
}
