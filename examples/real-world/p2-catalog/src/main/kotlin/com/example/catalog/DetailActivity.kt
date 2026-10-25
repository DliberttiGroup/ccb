package com.example.catalog

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.TextView

class DetailActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_detail)
    val id = intent.getStringExtra("product_id") ?: "p1"
    val name = when (id) {
      "p2" -> "Blue jacket"
      "p3" -> "Green hat"
      "p4" -> "Black bag"
      "p5" -> "White shirt"
      "p6" -> "Yellow scarf"
      "p7" -> "Brown boots"
      "p8" -> "Grey gloves"
      else -> "Red shoes"
    }
    findViewById<TextView>(R.id.detail_title).text = name
    findViewById<TextView>(R.id.detail_price).text = "$19.99"
    findViewById<Button>(R.id.add_button).setOnClickListener {
      startActivity(Intent(this, CartActivity::class.java))
    }
    findViewById<Button>(R.id.back_button).setOnClickListener { finish() }
  }
}
