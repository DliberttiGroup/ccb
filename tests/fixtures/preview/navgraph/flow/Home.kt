fun Home() {
  Column { Button("A") Button("B") Button("C") Button("D") Button("E") }
  aButton.setOnClickListener { navController.navigate(R.id.detail) }
  bButton.setOnClickListener { findNavController().navigate(R.id.form) }
  cButton.setOnClickListener { navController.navigate(R.id.missing) }
  dButton.setOnClickListener { navController.navigate("Detail") }
  navController.navigate(R.id.settings)
}
fun Detail() {
  Column { Text("Detail") }
}
fun Form() {
  Column { Text("Form") }
}
fun Settings() {
  Column { Text("Settings") }
}
