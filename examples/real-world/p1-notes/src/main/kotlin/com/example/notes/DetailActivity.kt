package com.example.notes

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.TextView

class DetailActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_detail)
    val id = intent.getStringExtra("note_id") ?: "note-1"
    val title = when (id) {
      "note-2" -> "Call Ana"
      "note-3" -> "Ideas"
      else -> "Buy milk"
    }
    findViewById<TextView>(R.id.detail_title).text = title
    findViewById<TextView>(R.id.detail_body).text = "Body of " + title
    findViewById<Button>(R.id.edit_button).setOnClickListener {
      val intent = Intent(this, FormActivity::class.java)
      intent.putExtra("note_id", id)
      startActivity(intent)
    }
    findViewById<Button>(R.id.delete_button).setOnClickListener { finish() }
    findViewById<Button>(R.id.back_button).setOnClickListener { finish() }
  }
}
