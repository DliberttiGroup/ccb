fun Home() {
  Column { Button("Open note") Button("New note") Button("Silent") }
}
fun onOpenClick(v: View) { nav("Detail") }
fun onNewClick(v: View) { if (ready) { nav("Form") } }
fun onGhostClick(v: View) { nav("Detail") }
fun onSilentClick(v: View) { }
fun Detail() {
  Column { Text("Detail") }
}
fun Form() {
  Column { Text("Form") }
}
