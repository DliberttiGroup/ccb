package com.example.catalog

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.widget.Button
import androidx.recyclerview.widget.RecyclerView

class MainActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_main)
    val products = listOf(
      R.id.product1_button to "p1",
      R.id.product2_button to "p2",
      R.id.product3_button to "p3",
      R.id.product4_button to "p4",
      R.id.product5_button to "p5",
      R.id.product6_button to "p6",
      R.id.product7_button to "p7",
      R.id.product8_button to "p8"
    )
    for ((buttonId, productId) in products) {
      findViewById<Button>(buttonId).setOnClickListener { openDetail(productId) }
    }
    findViewById<Button>(R.id.cart_button).setOnClickListener {
      startActivity(Intent(this, CartActivity::class.java))
    }
  }

  private fun openDetail(id: String) {
    val intent = Intent(this, DetailActivity::class.java)
    intent.putExtra("product_id", id)
    startActivity(intent)
  }
}
