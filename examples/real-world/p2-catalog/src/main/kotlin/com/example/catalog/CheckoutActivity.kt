package com.example.catalog

import android.app.Activity
import android.os.Bundle
import android.widget.Button

class CheckoutActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_checkout)
    findViewById<Button>(R.id.order_button).setOnClickListener { finish() }
    findViewById<Button>(R.id.back_button).setOnClickListener { finish() }
  }
}
