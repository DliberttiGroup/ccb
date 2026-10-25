package com.example.catalog

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.widget.Button

class CartActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_cart)
    findViewById<Button>(R.id.checkout_button).setOnClickListener {
      startActivity(Intent(this, CheckoutActivity::class.java))
    }
    findViewById<Button>(R.id.continue_button).setOnClickListener { finish() }
  }
}
