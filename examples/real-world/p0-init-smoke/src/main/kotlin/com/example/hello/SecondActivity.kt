package com.example.hello

import android.app.Activity
import android.os.Bundle
import android.widget.Button

class SecondActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_second)
    findViewById<Button>(R.id.back_button).setOnClickListener { finish() }
  }
}
