package com.example.messy

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Button

class MainActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_main)
    findViewById<Button>(R.id.open_button).setOnClickListener {
      startActivity(Intent(this, DetailActivity::class.java))
    }
  }

  fun onShareClick(v: View) {
    startActivity(Intent(this, SettingsActivity::class.java))
  }
}
