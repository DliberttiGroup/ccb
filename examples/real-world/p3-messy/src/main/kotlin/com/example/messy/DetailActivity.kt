package com.example.messy

import android.app.Activity
import android.os.Bundle
import android.widget.Button

class DetailActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_detail)
    findViewById<Button>(R.id.back_button).setOnClickListener { finish() }
  }
}
