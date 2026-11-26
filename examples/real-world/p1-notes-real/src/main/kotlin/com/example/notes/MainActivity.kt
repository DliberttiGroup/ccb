package com.example.notes

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Button

class MainActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_main)
    findViewById<Button>(R.id.note1_button).setOnClickListener { openDetail("note-1") }
    findViewById<Button>(R.id.note2_button).setOnClickListener { openDetail("note-2") }
    findViewById<Button>(R.id.note3_button).setOnClickListener { openDetail("note-3") }
    findViewById<Button>(R.id.new_note_button).setOnClickListener {
      startActivity(Intent(this, FormActivity::class.java))
    }
  }

  fun onSettingsClick(v: View) {
    startActivity(Intent(this, SettingsActivity::class.java))
  }

  private fun openDetail(id: String) {
    val intent = Intent(this, DetailActivity::class.java)
    intent.putExtra("note_id", id)
    startActivity(intent)
  }
}
